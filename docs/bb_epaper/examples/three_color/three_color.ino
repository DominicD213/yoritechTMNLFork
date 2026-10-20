//
// 3 color (Black/White/Red) example
//
#include <bb_epaper.h>
#include "smiley_bwr.h"
//BBEPAPER bbep(EP154R_152x152); // 1.54" 152x152 B/W/R
BBEPAPER bbep(EP26R_152x296);
//BBEPAPER bbep(EP266YR_184x360); // 1.54" 152x152 B/W/R
// My Arduino Nano 33 BLE e-paper adapter
//#define DC_PIN 16
//#define BUSY_PIN 15
//#define RESET_PIN 14
//#define CS_PIN 10
// My Xiao + LCD Adapter
#define DC_PIN 4
#define BUSY_PIN 20
#define RESET_PIN 5
#define CS_PIN 21

void setup()
{
  bbep.initIO(DC_PIN, RESET_PIN, BUSY_PIN, CS_PIN, -1, -1);
  bbep.allocBuffer(); // use a back buffer
 // bbep.setRotation(270);
  bbep.fillScreen(BBEP_WHITE);
  bbep.setTextColor(BBEP_RED);
  bbep.setFont(FONT_12x16);
  bbep.println("bb_epaper v1");
  bbep.setTextColor(BBEP_BLACK);
  bbep.println("B/W/R Test");
  for (int i=1; i<44; i++) {
    bbep.drawCircle(bbep.width()/2, 64, i, (i < 22) ? BBEP_BLACK : BBEP_RED);
  }
  bbep.loadG5Image(smiley_bwr, 0, 160, 0,0,1.0f);
  bbep.writePlane();
  bbep.refresh(REFRESH_FULL);
  bbep.wait();
  bbep.sleep(DEEP_SLEEP);
}

void loop()
{

}