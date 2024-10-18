/* use SparkFun BNO080 library (which uses stuff from Arduino) to get
    orientation of Complex Arts Sensorboard */

#include "../arduino/SparkFun_BNO080_Arduino_Library.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gyro.h"
#include "espd.h"

#ifdef PD_USE_GYRO

/* IMU device */
BNO080 myIMU;

/* SPI device */
SPIClass spiPort;
uint32_t spiPortSpeed = 100*1000;

/* IMU pins */
byte imuCSPin = PIN_IMUCSP;
byte imuWAKPin = PIN_IMUWAK;
byte imuINTPin = PIN_IMUINT;
byte imuRSTPin = PIN_IMURST;

/* SPI pins */
byte spiCLK = PIN_SPICLK;
byte spiMISO = PIN_SPIMISO;
byte spiMOSI = PIN_SPIMOSI;

char TAG[] = "gyro";

extern "C" void gyro_poll(void)
{
    if (myIMU.dataAvailable() == true)
    {
        /* pd_sendgyro( myIMU.getRoll(), myIMU.getPitch(), myIMU.getYaw()); */
        pd_sendgyro(myIMU.getAccelX(), myIMU.getAccelY(), myIMU.getAccelZ());
    }
}

extern "C" int gyro_init(void)
{
        /* initialize SPI interface for the BNO085 */
    spiPort.begin(spiCLK, spiMISO, spiMOSI, imuCSPin);

        /* initialize IMU */
    if (myIMU.beginSPI(imuCSPin, imuWAKPin, imuINTPin, imuRSTPin,
        spiPortSpeed, spiPort) == false)
            return (0);

        /* enable "rotation vector", readings will be produced every 50 ms */
    /* myIMU.enableRotationVector(50); */

        /* enable accelerometer */
    myIMU.enableAccelerometer(50);
    
    return (1);
}

#endif /* PD_USEGYRO */
