/*

Module: catena-pms7003-lora.ino

Function:
    Sensor sketch measuring and transmitting air-quality info.

Copyright:
    See accompanying LICENSE file for copyright and license information.

Author:
    Terry Moore, MCCI Corporation   July 2019

*/

#ifndef ARDUINO_MCCI_CATENA_4630
# error "This sketch targets the MCCI Catena 4630"
#endif

#include <Arduino.h>
#include "catena-pms7003-lora-cMeasurementLoop.h"

#include <Wire.h>
#include <Catena.h>
#include <Catena_Led.h>
#include <Catena_Log.h>
#include <Catena_Mx25v8035f.h>
#include <Catena_PollableInterface.h>
#include <Catena_TxBuffer.h>
#include <Catena-SHT3x.h>
#include <arduino_lmic.h>
#include <Catena-PMS7003.h>
#include <Catena-PMS7003Hal-4630.h>
#include <sgpc3.h>
#include <mcciadk_baselib.h>
#include <stdlib.h>

#include <cstdint>

using namespace McciCatena;
using namespace McciCatenaPMS7003;
using namespace McciCatenaSht3x;

/****************************************************************************\
|
|   Constants.
|
\****************************************************************************/

constexpr std::uint32_t kAppVersion = McciCatenaPMS7003::makeVersion(2,0,0,0);

/****************************************************************************\
|
|   Variables.
|
\****************************************************************************/

Catena gCatena;
Catena::LoRaWAN gLoRaWAN;
StatusLed gLed (Catena::PIN_STATUS_LED);

SPIClass gSPI2(
    Catena::PIN_SPI2_MOSI,
    Catena::PIN_SPI2_MISO,
    Catena::PIN_SPI2_SCK
    );

//   The flash
Catena_Mx25v8035f gFlash;
bool gfFlash;

//  The Temperature/humidity sensor
cSHT3x gTempRh { Wire };

// true if SHT3x is running.
bool gfTempRh;

//  The Air Quality sensor
SGPC3 gSgpc3Sensor;

// true if SGPC3 is running.
bool gfSgpc3;

// the HAL for the PMS7003 library.
cPMS7003Hal_4630 gPmsHal 
    { 
    gCatena,
    (cPMS7003::DebugFlags::kError |
     cPMS7003::DebugFlags::kTrace)
    };

// the PMS7003 instance
cPMS7003 gPms7003 { Serial2, gPmsHal };

// the measurement loop instance
cMeasurementLoop gMeasurementLoop { gPms7003, gTempRh, gSgpc3Sensor };

// forward reference to the command functions
cCommandStream::CommandFn cmdDebugMask;
cCommandStream::CommandFn cmdRunStop;
cCommandStream::CommandFn cmdStats;

// the individual commmands are put in this table
static const cCommandStream::cEntry sMyExtraCommmands[] =
        {
        { "debugmask", cmdDebugMask },
        { "run", cmdRunStop },
        { "stats", cmdStats },
        { "stop", cmdRunStop },
        // other commands go here....
        };

/* a top-level structure wraps the above and connects to the system table */
/* it optionally includes a "first word" so you can for sure avoid name clashes */
static cCommandStream::cDispatch
sMyExtraCommands_top(
        sMyExtraCommmands,          /* this is the pointer to the table */
        sizeof(sMyExtraCommmands),  /* this is the size of the table */
        nullptr                     /* this is no "first word" for all the commands in this table */
        );

/****************************************************************************\
|
|   Setup
|
\****************************************************************************/

void setup()
    {
    setup_platform();
    setup_printSignOn();

    setup_flash();
    setup_sensors();
    setup_radio();
    setup_pms7003();
    setup_commands();
    setup_measurement();
    }

void setup_platform()
    {
    gCatena.begin();

    gLed.begin();
    gCatena.registerObject(&gLed);
    gLed.Set(McciCatena::LedPattern::FastFlash);

    // if running unattended, don't wait for USB connect.
    if (! (gCatena.GetOperatingFlags() &
            static_cast<uint32_t>(gCatena.OPERATING_FLAGS::fUnattended)))
            {
            while (!Serial)
                    /* wait for USB attach */
                    yield();
            }

    gLed.Set(McciCatena::LedPattern::FiftyFiftySlow);
    }

static constexpr const char *filebasename(const char *s)
    {
    const char *pName = s;

    for (auto p = s; *p != '\0'; ++p)
        {
        if (*p == '/' || *p == '\\')
            pName = p + 1;
        }
    return pName;
    }

void setup_printSignOn()
    {
    static const char dashes[] = "------------------------------------";

    gCatena.SafePrintf("\n%s%s\n", dashes, dashes);

    gCatena.SafePrintf("This is %s v%d.%d.%d.%d.\n",
        filebasename(__FILE__),
        McciCatenaPMS7003::getMajor(kAppVersion),
        McciCatenaPMS7003::getMinor(kAppVersion),
        McciCatenaPMS7003::getPatch(kAppVersion),
        McciCatenaPMS7003::getLocal(kAppVersion)
        );

    do  {
        char sRegion[16];
        gCatena.SafePrintf("Target network: %s / %s\n",
                        gLoRaWAN.GetNetworkName(),
                        gLoRaWAN.GetRegionString(sRegion, sizeof(sRegion))
                        );
        } while (0);

    gCatena.SafePrintf("System clock rate is %u.%03u MHz\n",
        ((unsigned)gCatena.GetSystemClockRate() / (1000*1000)),
        ((unsigned)gCatena.GetSystemClockRate() / 1000 % 1000)
        );
    gCatena.SafePrintf("Enter 'help' for a list of commands.\n");
    gCatena.SafePrintf("(remember to select 'Line Ending: Newline' at the bottom of the monitor window.)\n");

    gCatena.SafePrintf("%s%s\n" "\n", dashes, dashes);
    }

void setup_flash(void)
    {
    if (gFlash.begin(&gSPI2, Catena::PIN_SPI2_FLASH_SS))
        {
        gfFlash = true;
        gFlash.powerDown();
        gCatena.SafePrintf("FLASH found, put power down\n");
        }
    else
        {
        gfFlash = false;
        gFlash.end();
        gSPI2.end();
        gCatena.SafePrintf("No FLASH found: check hardware\n");
        }
    }

void setup_radio()
    {
    gLoRaWAN.begin(&gCatena);
    gCatena.registerObject(&gLoRaWAN);
    LMIC_setClockError(5 * MAX_CLOCK_ERROR / 100);
    }

void setup_sensors()
    {
    Wire.begin();
    gMeasurementLoop.setTempRh(gTempRh.begin());

    pinMode(D5, OUTPUT);
    digitalWrite(D5, HIGH);

    // initialize SGPC3 Air Quality sensor
    // Set Ultra Low power mode, sampling every 30s
    gSgpc3Sensor.ultraLowPower();
    if (!gSgpc3Sensor.initSGPC3(LT_FOREVER))
        {
        gMeasurementLoop.setSgpc3(true);

        // Read the spec of the sensor, type and version
        uint8_t type = gSgpc3Sensor.getProductType();
        uint8_t p_version = gSgpc3Sensor.getVersion();
        gCatena.SafePrintf("Type: %d\n", type);
        gCatena.SafePrintf("Version: %d\n", p_version);
        }
    else
        {
        gMeasurementLoop.setSgpc3(false);
        gCatena.SafePrintf("Error while initializing sensors: %d",
                gSgpc3Sensor.getError()
                );
        }
    }

void setup_pms7003()
    {
    gPms7003.begin();
    gMeasurementLoop.begin();
    }

void setup_commands()
    {
    /* add our application-specific commands */
    gCatena.addCommands(
        /* name of app dispatch table, passed by reference */
        sMyExtraCommands_top,
        /*
        || optionally a context pointer using static_cast<void *>().
        || normally only libraries (needing to be reentrant) need
        || to use the context pointer.
        */
        nullptr
        );
    }

void setup_measurement()
    {
    if (gLoRaWAN.IsProvisioned())
        gMeasurementLoop.requestActive(true);
    else
        {
        gCatena.SafePrintf("not provisioned, idling\n");
        gMeasurementLoop.requestActive(false);
        }
    }

/****************************************************************************\
|
|   Loop
|
\****************************************************************************/

void loop()
    {
    gCatena.poll();
    }
