// #include "BluetoothSerial.h"

// BluetoothSerial SerialBT;

// // Motor pins
// int IN1 = 26;
// int IN2 = 27;
// int IN3 = 14;
// int IN4 = 12;

// void setup() {
//   Serial.begin(115200);
//   SerialBT.begin("ESP32_CAR");

//   pinMode(IN1, OUTPUT);
//   pinMode(IN2, OUTPUT);
//   pinMode(IN3, OUTPUT);
//   pinMode(IN4, OUTPUT);
// }

// void loop() {
//   if (SerialBT.available()) {
//     char cmd = SerialBT.read();

//     if (cmd == 'F') { // Forward
//       digitalWrite(IN1, HIGH);
//       digitalWrite(IN2, LOW);
//       digitalWrite(IN3, HIGH);
//       digitalWrite(IN4, LOW);
//     }
//     else if (cmd == 'B') { // Backward
//       digitalWrite(IN1, LOW);
//       digitalWrite(IN2, HIGH);
//       digitalWrite(IN3, LOW);
//       digitalWrite(IN4, HIGH);
//     }
//     else if (cmd == 'L') { // Left
//       digitalWrite(IN1, LOW);
//       digitalWrite(IN2, HIGH);
//       digitalWrite(IN3, HIGH);
//       digitalWrite(IN4, LOW);
//     }
//     else if (cmd == 'R') { // Right
//       digitalWrite(IN1, HIGH);
//       digitalWrite(IN2, LOW);
//       digitalWrite(IN3, LOW);
//       digitalWrite(IN4, HIGH);
//     }
//     else if (cmd == 'S') { // Stop
//       digitalWrite(IN1, LOW);
//       digitalWrite(IN2, LOW);
//       digitalWrite(IN3, LOW);
//       digitalWrite(IN4, LOW);
//     }
//   }
// }


///////////////////////////////////////////////////////////////////////////////////////////////              
// base code
#include <ESP32Servo.h> // Use ESP32Servo for better timer management on ESP32
#include <Wire.h>
#include "Adafruit_VL6180X.h"

#include <AnimatedGIF.h>
#include <SPI.h>
#include <TFT_eSPI.h>

#include "sideview.h"
#include "sleep.h"

#include "BluetoothSerial.h"


#define DISPLAY_WIDTH  tft.width()
#define DISPLAY_HEIGHT tft.height()
#define BUFFER_SIZE 256            // Optimum is >= GIF width or integral division of width

#define M1  26    
#define M2  27
#define M3  25 //14
#define M4  33 // 12

#define enb1  14 //25
#define enb2  12 // 33

#define sensorLIP 35  // 
#define sensorRIP 32  // 

TFT_eSPI tft = TFT_eSPI();
AnimatedGIF gif;
Adafruit_VL6180X vl = Adafruit_VL6180X();
Servo myservo;
BluetoothSerial SerialBT;

// Structure to hold GIF data + size
struct GifItem {
  const uint8_t* data;
  size_t size;
};

// Add all GIFs here
GifItem gifs[] = {
  {sideview, sizeof(sideview)},
  {sleepy, sizeof(sleepy)}
};

int gifCount = sizeof(gifs) / sizeof(gifs[0]);

#ifdef USE_DMA
  uint16_t usTemp[2][BUFFER_SIZE]; // Global to support DMA use
#else
  uint16_t usTemp[1][BUFFER_SIZE];    // Global to support DMA use
#endif
bool dmaBuf = 0;

uint8_t ref = 100;
uint8_t ligref = 25;
int servoPin = 16; // Valid PWM GPIO pin (e.g., 18)
int pos = 0;

bool FlagRun = true;
bool FlagLight = true;
char mode;
char Data;

// Draw a line of image directly on the LCD
void GIFDraw(GIFDRAW *pDraw)
{
  uint8_t *s;
  uint16_t *d, *usPalette;
  int x, y, iWidth, iCount;

  // Displ;ay bounds chech and cropping
  iWidth = pDraw->iWidth;
  if (iWidth + pDraw->iX > DISPLAY_WIDTH)
    iWidth = DISPLAY_WIDTH - pDraw->iX;
  usPalette = pDraw->pPalette;
  y = pDraw->iY + pDraw->y; // current line
  if (y >= DISPLAY_HEIGHT || pDraw->iX >= DISPLAY_WIDTH || iWidth < 1)
    return;

  // Old image disposal
  s = pDraw->pPixels;
  if (pDraw->ucDisposalMethod == 2) // restore to background color
  {
    for (x = 0; x < iWidth; x++)
    {
      if (s[x] == pDraw->ucTransparent)
        s[x] = pDraw->ucBackground;
    }
    pDraw->ucHasTransparency = 0;
  }

  // Apply the new pixels to the main image
  if (pDraw->ucHasTransparency) // if transparency used
  {
    uint8_t *pEnd, c, ucTransparent = pDraw->ucTransparent;
    pEnd = s + iWidth;
    x = 0;
    iCount = 0; // count non-transparent pixels
    while (x < iWidth)
    {
      c = ucTransparent - 1;
      d = &usTemp[0][0];
      while (c != ucTransparent && s < pEnd && iCount < BUFFER_SIZE )
      {
        c = *s++;
        if (c == ucTransparent) // done, stop
        {
          s--; // back up to treat it like transparent
        }
        else // opaque
        {
          *d++ = usPalette[c];
          iCount++;
        }
      } // while looking for opaque pixels
      if (iCount) // any opaque pixels?
      {
        // DMA would degrtade performance here due to short line segments
        tft.setAddrWindow(pDraw->iX + x, y, iCount, 1);
        tft.pushPixels(usTemp, iCount);
        x += iCount;
        iCount = 0;
      }
      // no, look for a run of transparent pixels
      c = ucTransparent;
      while (c == ucTransparent && s < pEnd)
      {
        c = *s++;
        if (c == ucTransparent)
          x++;
        else
          s--;
      }
    }
  }
  else
  {
    s = pDraw->pPixels;

    // Unroll the first pass to boost DMA performance
    // Translate the 8-bit pixels through the RGB565 palette (already byte reversed)
    if (iWidth <= BUFFER_SIZE)
      for (iCount = 0; iCount < iWidth; iCount++) usTemp[dmaBuf][iCount] = usPalette[*s++];
    else
      for (iCount = 0; iCount < BUFFER_SIZE; iCount++) usTemp[dmaBuf][iCount] = usPalette[*s++];

#ifdef USE_DMA // 71.6 fps (ST7796 84.5 fps)
    tft.dmaWait();
    tft.setAddrWindow(pDraw->iX, y, iWidth, 1);
    tft.pushPixelsDMA(&usTemp[dmaBuf][0], iCount);
    dmaBuf = !dmaBuf;
#else // 57.0 fps
    tft.setAddrWindow(pDraw->iX, y, iWidth, 1);
    tft.pushPixels(&usTemp[0][0], iCount);
#endif

    iWidth -= iCount;
    // Loop if pixel buffer smaller than width
    while (iWidth > 0)
    {
      // Translate the 8-bit pixels through the RGB565 palette (already byte reversed)
      if (iWidth <= BUFFER_SIZE)
        for (iCount = 0; iCount < iWidth; iCount++) usTemp[dmaBuf][iCount] = usPalette[*s++];
      else
        for (iCount = 0; iCount < BUFFER_SIZE; iCount++) usTemp[dmaBuf][iCount] = usPalette[*s++];

#ifdef USE_DMA
      tft.dmaWait();
      tft.pushPixelsDMA(&usTemp[dmaBuf][0], iCount);
      dmaBuf = !dmaBuf;
#else
      tft.pushPixels(&usTemp[0][0], iCount);
#endif
      iWidth -= iCount;
    }
  }
} /* GIFDraw() */

void gifPlayer(const uint8_t* data, size_t size){
   int frames = 0;
  long startTime = micros();

  if (gif.open((uint8_t*)data, size, GIFDraw))
  {
    tft.startWrite();

    while (gif.playFrame(false, NULL))
    {
      frames++;
      delay(50);
      yield();  // keep ESP32 stable
    }

    gif.close();
    tft.endWrite();

    long duration = micros() - startTime;

    Serial.print("FPS: ");
    Serial.println(frames / (duration / 1000000.0));
  }
 }

 void gif_loop(){
  
 for (int i = 0; i < gifCount; i++)
  {
    gifPlayer(gifs[i].data, gifs[i].size);
    // delay(200); // small gap between GIFs
  }
  
}

void trunck(){
  uint8_t range = vl.readRange();
  uint8_t status = vl.readRangeStatus();  

    if (status == VL6180X_ERROR_NONE) {
    Serial.print("Range: "); Serial.println(range);
  }

  if((range > ref%2) && (range <= ref)) { // open trank for 5 sec at 100 degree
    myservo.write(100);
    delay(5000);
     myservo.write(0);
  }
}

void lightLogic()
{
  uint8_t val = vl.readLux(VL6180X_ALS_GAIN_5);

  Serial.println(val);

  if (val < ligref) // 25 dark side
  {
    FlagLight = false;
  }else if (val > ligref) // light 
  {
    FlagLight = true;

  }
  
  
  // // DARK → sleepy
  // if (val <= threshold && !sleepyPlayed)
  // {
  //   Serial.println("Sleepy");
  //   gifPlayer(gifs[1].data, gifs[1].size);
  //   sleepyPlayed = true;
  //   sidePlayed = false;
  // }

  // // BRIGHT → sideview
  // if (val >= threshold && !sidePlayed)
  // {
  //   Serial.println("Sideview");
  //   gifPlayer(gifs[0].data, gifs[0].size);
  //   sidePlayed = true;
  //   sleepyPlayed = false;
  // }
}

void forward(){
  digitalWrite(M1,HIGH);
  digitalWrite(M2,LOW);
  digitalWrite(M3,HIGH);
  digitalWrite(M4,LOW); 
}

void left(){
  digitalWrite(M1,LOW);
  digitalWrite(M2,HIGH);
  digitalWrite(M3,HIGH);
  digitalWrite(M4,LOW); 
}

void right(){
  digitalWrite(M1,HIGH);
  digitalWrite(M2,LOW);
  digitalWrite(M3,LOW);
  digitalWrite(M4,HIGH); 
}

void backward(){

  digitalWrite(M1,LOW);
  digitalWrite(M2,HIGH);
  digitalWrite(M3,LOW);
  digitalWrite(M4,HIGH); 
 
}

void stop(){
  digitalWrite(M1,LOW);
  digitalWrite(M2,LOW);
  digitalWrite(M3,LOW);
  digitalWrite(M4,LOW);
}

void car_motion(){

  int sensorL = digitalRead(sensorLIP);
  int sensorR = digitalRead(sensorRIP);

  digitalWrite(enb1,HIGH);
  digitalWrite(enb2,HIGH);
  
  if(sensorL == 0 && sensorR == 0){
    // Serial.println("F");
    FlagRun = false;
    forward();
    return ;
  }
   else if(sensorL == 0 && sensorR == 1){
    FlagRun = false;
    left();
    return ;
  }
    else if(sensorL == 1 && sensorR == 0){
      FlagRun = false;
      right();
    return ;
  }
    else if(sensorL == 1 && sensorR == 1){
      FlagRun = true;
      stop(); 
    return ;
  }
  else{
    // FlagRun = false;
  }
}


void bluetoothCar(char command){

  if(command == 'F'){
    Serial.println("F");
    forward();
    return ;
  }
   else if(command == 'L'){
    left();
    return ;
  }
    else if(command == 'R'){
      right();
    return ;
  }
    else if(command == 'B'){
      backward(); 
    return ;
  }else if(command == 'S'){
      stop(); 
    return ;
  }
  else{
  }

}

void Task1(void *pvParameters) {
    while (true) {
        Serial.println("Task 1 car");

        if (SerialBT.available() > 0)
        {
          Data = SerialBT.read();
          if (Data == 'A'){
            mode = 'A';
          }
          else if (Data == 'M'){
            mode = 'M';
          }else if (mode == 'A')
          {
            bluetoothCar(Data);
          }
          // else if (mode == 'M')
          // {
          //   // bluetoothCar(Data);
          //   return;
          // }
        }

        if (mode == 'M')
        {
          lightLogic();
          trunck();
          car_motion();
          /* code */
        }
        
        // vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void Task2(void *pvParameters) {
    while (true) {
        Serial.println("Task 2 gif");
        // gif_loop();

        if (FlagRun == true && FlagLight == true){ // M = off && Lig = on 
          tft.setCursor(0,0);
          tft.setTextSize(12);
          tft.setTextColor(TFT_WHITE);
          tft.print("Please Through Garbage");
        }else if (FlagRun == false && FlagLight == true){ // M = on && Lig = on 
          gifPlayer(gifs[0].data, gifs[0].size); /// side gif play 
        }else if (FlagRun == true && FlagLight == false) // M = off && Lig = off
        {
          gifPlayer(gifs[1].data, gifs[1].size); /// sleep gif play
          /* code */
        }else if (FlagRun == true && FlagLight == false) //M = on && Lig = off
        {
          gifPlayer(gifs[1].data, gifs[1].size);
          /* code */
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}

void setup() {
  Serial.begin(115200);
  pinMode(enb1,OUTPUT);
  pinMode(enb2,OUTPUT);
  
  pinMode(M1,OUTPUT);
  pinMode(M2,OUTPUT);
  pinMode(M3,OUTPUT);
  pinMode(M4,OUTPUT);

  pinMode(sensorLIP,INPUT);
  pinMode(sensorRIP,INPUT);

  myservo.attach(servoPin);
  if (! vl.begin()) {
    Serial.println("Failed to find sensor");
    while (1);
  }
  myservo.write(0);

  SerialBT.begin("ESP32_CAR");

  tft.begin();
#ifdef USE_DMA
  tft.initDMA();
#endif
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  tft.fillScreen(TFT_RED);
  delay(2000);
  tft.fillScreen(TFT_GREEN);
  delay(2000);
  tft.fillScreen(TFT_BLUE);
  delay(2000);
  gif.begin(BIG_ENDIAN_PIXELS);

  xTaskCreate(
        Task1,        // function
        "Task1",      // name
        2048,         // stack size
        NULL,
        1,            // priority
        NULL
    );

    xTaskCreate(
        Task2,
        "Task2",
        2048,
        NULL,
        1,
        NULL
    );

}

void loop() {
  
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// multitasking working without BLE
// #include <ESP32Servo.h>
// #include <Wire.h>
// #include "Adafruit_VL6180X.h"

// #include <AnimatedGIF.h>
// #include <SPI.h>
// #include <TFT_eSPI.h>

// #include "sideview.h"
// #include "sleep.h"

// // ------------------- PINS -------------------
// #define M1  26    
// #define M2  27
// #define M3  14
// #define M4  12

// #define enb1  25
// #define enb2  33

// #define sensorLIP 35
// #define sensorRIP 32

// // ------------------- OBJECTS -------------------
// TFT_eSPI tft = TFT_eSPI();
// AnimatedGIF gif;
// Adafruit_VL6180X vl = Adafruit_VL6180X();
// Servo myservo;

// // ------------------- GIF STRUCT -------------------
// struct GifItem {
//   const uint8_t* data;
//   size_t size;
// };

// GifItem gifs[] = {
//   {sideview, sizeof(sideview)},
//   {sleepy, sizeof(sleepy)}
// };

// int gifCount = sizeof(gifs) / sizeof(gifs[0]);

// #define BUFFER_SIZE 256
// uint16_t usTemp[1][BUFFER_SIZE];

// uint8_t ref = 100;
// int servoPin = 16;

// // ------------------- GIF DRAW -------------------
// void GIFDraw(GIFDRAW *pDraw)
// {
//   uint8_t *s = pDraw->pPixels;
//   uint16_t *d = usTemp[0];
//   uint16_t *palette = pDraw->pPalette;

//   int x = pDraw->iX;
//   int y = pDraw->iY + pDraw->y;
//   int width = pDraw->iWidth;

//   if (y >= tft.height()) return;

//   for (int i = 0; i < width; i++) {
//     d[i] = palette[*s++];
//   }

//   tft.setAddrWindow(x, y, width, 1);
//   tft.pushPixels(d, width);
// }

// // ------------------- GIF PLAYER -------------------
// void gifPlayer(const uint8_t* data, size_t size)
// {
//   if (gif.open((uint8_t*)data, size, GIFDraw))
//   {
//     tft.startWrite();

//     while (gif.playFrame(false, NULL))
//     {
//       delay(30);   // SAFE here (loop context)
//       yield();
//     }

//     gif.close();
//     tft.endWrite();
//   }
// }

// void gif_loop()
// {
//   for (int i = 0; i < gifCount; i++)
//   {
//     gifPlayer(gifs[i].data, gifs[i].size);
//     delay(200);
//   }
// }

// // ------------------- SERVO NON-BLOCKING -------------------
// void trunck()
// {
//   static bool open = false;
//   static unsigned long startTime = 0;

//   uint8_t range = vl.readRange();
//   uint8_t status = vl.readRangeStatus();

//   if ((range > 2) && (range <= ref) && !open) {
//     myservo.write(100);
//     startTime = millis();
//     open = true;
//   }

//   if (open && (millis() - startTime > 5000)) {
//     myservo.write(0);
//     open = false;
//   }
// }

// // ------------------- MOTOR CONTROL -------------------
// void forward(){
//   digitalWrite(M1,HIGH);
//   digitalWrite(M2,LOW);
//   digitalWrite(M3,HIGH);
//   digitalWrite(M4,LOW); 
// }

// void left(){
//   digitalWrite(M1,LOW);
//   digitalWrite(M2,HIGH);
//   digitalWrite(M3,HIGH);
//   digitalWrite(M4,LOW); 
// }

// void right(){
//   digitalWrite(M1,HIGH);
//   digitalWrite(M2,LOW);
//   digitalWrite(M3,LOW);
//   digitalWrite(M4,HIGH); 
// }

// void backward(){
//   digitalWrite(M1,LOW);
//   digitalWrite(M2,LOW);
//   digitalWrite(M3,LOW);
//   digitalWrite(M4,LOW); 
// }

// void car_motion()
// {
//   int sensorL = digitalRead(sensorLIP);
//   int sensorR = digitalRead(sensorRIP);

//   digitalWrite(enb1,HIGH);
//   digitalWrite(enb2,HIGH);

//   if(sensorL == 0 && sensorR == 0){
//     forward();
//   }
//   else if(sensorL == 0 && sensorR == 1){
//     left();
//   }
//   else if(sensorL == 1 && sensorR == 0){
//     right();
//   }
//   else{
//     backward();
//   }
// }

// // ------------------- RTOS TASK (CAR ONLY) -------------------
// void TaskCar(void *pvParameters)
// {
//   while (true)
//   {
//     car_motion();
//     // trunck();

//     vTaskDelay(50 / portTICK_PERIOD_MS); // IMPORTANT
//   }
// }

// // ------------------- SETUP -------------------
// void setup()
// {
//   Serial.begin(115200);

//   pinMode(enb1,OUTPUT);
//   pinMode(enb2,OUTPUT);

//   pinMode(M1,OUTPUT);
//   pinMode(M2,OUTPUT);
//   pinMode(M3,OUTPUT);
//   pinMode(M4,OUTPUT);

//   pinMode(sensorLIP,INPUT);
//   pinMode(sensorRIP,INPUT);

//   myservo.attach(servoPin);

//   if (!vl.begin()) {
//     Serial.println("Sensor fail");
//     while (1);
//   }

//   myservo.write(0);

//   tft.begin();
//   tft.setRotation(1);
//   tft.fillScreen(TFT_BLACK);

//   gif.begin(BIG_ENDIAN_PIXELS);

//   // ONLY CAR TASK (RTOS SAFE)
//   xTaskCreatePinnedToCore(
//     TaskCar,
//     "CarTask",
//     2048,
//     NULL,
//     2,
//     NULL,
//     0   // Core 0
//   );
// }

// // ------------------- LOOP (DISPLAY SAFE ZONE) -------------------
// void loop()
// {
//   // gif_loop();   // DISPLAY RUNS HERE (NO RTOS ISSUES)

//    gifPlayer(gifs[0].data, gifs[0].size);


// }


// /////////////////////////////////////////////////////////////////////////////////////////

// multitasking code
// #include <ESP32Servo.h>
// #include <Wire.h>
// #include "Adafruit_VL6180X.h"

// #include <AnimatedGIF.h>
// #include <SPI.h>
// #include <TFT_eSPI.h>

// #include "sideview.h"
// #include "sleep.h"

// // ---------------- BLE ----------------
// #include <BLEDevice.h>
// #include <BLEServer.h>
// #include <BLEUtils.h>
// #include <BLE2902.h>

// BLECharacteristic *pCharacteristic;

// #define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
// #define CHARACTERISTIC_UUID "abcd1234-5678-1234-5678-abcdef123456"

// // ---------------- PINS ----------------
// #define M1  26    
// #define M2  27
// #define M3  14
// #define M4  12

// #define enb1  25
// #define enb2  33

// #define sensorLIP 35
// #define sensorRIP 32

// // ---------------- OBJECTS ----------------
// TFT_eSPI tft = TFT_eSPI();
// AnimatedGIF gif;
// Adafruit_VL6180X vl = Adafruit_VL6180X();
// Servo myservo;

// // ---------------- GIF DATA ----------------
// struct GifItem {
//   const uint8_t* data;
//   size_t size;
// };

// GifItem gifs[] = {
//   {sideview, sizeof(sideview)},
//   {sleepy, sizeof(sleepy)}
// };

// int gifCount = sizeof(gifs) / sizeof(gifs[0]);

// #define BUFFER_SIZE 256
// uint16_t usTemp[1][BUFFER_SIZE];

// uint8_t ref = 100;
// int servoPin = 16;

// // ---------------- CONTROL FLAGS ----------------
// bool manualMode = false;
// char lastCommand = 'S';

// // ---------------- MOTOR CONTROL ----------------
// void forward(){
//   digitalWrite(M1,HIGH);
//   digitalWrite(M2,LOW);
//   digitalWrite(M3,HIGH);
//   digitalWrite(M4,LOW); 
// }

// void left(){
//   digitalWrite(M1,LOW);
//   digitalWrite(M2,HIGH);
//   digitalWrite(M3,HIGH);
//   digitalWrite(M4,LOW); 
// }

// void right(){
//   digitalWrite(M1,HIGH);
//   digitalWrite(M2,LOW);
//   digitalWrite(M3,LOW);
//   digitalWrite(M4,HIGH); 
// }

// void backward(){
//   digitalWrite(M1,LOW);
//   digitalWrite(M2,LOW);
//   digitalWrite(M3,LOW);
//   digitalWrite(M4,LOW); 
// }

// // ---------------- SENSOR LOGIC ----------------
// void car_motion()
// {
//   if (manualMode) return; // skip auto mode

//   int sensorL = digitalRead(sensorLIP);
//   int sensorR = digitalRead(sensorRIP);

//   digitalWrite(enb1,HIGH);
//   digitalWrite(enb2,HIGH);

//   if(sensorL == 0 && sensorR == 0){
//     forward();
//   }
//   else if(sensorL == 0 && sensorR == 1){
//     left();
//   }
//   else if(sensorL == 1 && sensorR == 0){
//     right();
//   }
//   else{
//     backward();
//   }
// }

// // ---------------- TRUNK (NON BLOCKING) ----------------
// void trunck()
// {
//   static bool open = false;
//   static unsigned long startTime = 0;

//   uint8_t range = vl.readRange();

//   if ((range > 2) && (range <= ref) && !open) {
//     myservo.write(100);
//     startTime = millis();
//     open = true;
//   }

//   if (open && (millis() - startTime > 5000)) {
//     myservo.write(0);
//     open = false;
//   }
// }

// // ---------------- GIF PLAYER ----------------
// void GIFDraw(GIFDRAW *pDraw)
// {
//   uint8_t *s = pDraw->pPixels;
//   uint16_t *d = usTemp[0];
//   uint16_t *palette = pDraw->pPalette;

//   int x = pDraw->iX;
//   int y = pDraw->iY + pDraw->y;
//   int width = pDraw->iWidth;

//   if (y >= tft.height()) return;

//   for (int i = 0; i < width; i++) {
//     d[i] = palette[*s++];
//   }

//   tft.setAddrWindow(x, y, width, 1);
//   tft.pushPixels(d, width);
// }

// void gifPlayer(const uint8_t* data, size_t size)
// {
//   if (gif.open((uint8_t*)data, size, GIFDraw))
//   {
//     tft.startWrite();

//     while (gif.playFrame(false, NULL))
//     {
//       delay(50);
//       yield();
//     }

//     gif.close();
//     tft.endWrite();
//   }
// }

// bool Mstatus = false;
// // ---------------- BLE CALLBACK ----------------
// class MyCallbacks: public BLECharacteristicCallbacks {
//   void onWrite(BLECharacteristic *pCharacteristic) {
//     std::string value = pCharacteristic->getValue();

//     if (value.length() > 0) {
//       String cmd = String(value.c_str());
//       Serial.println("BLE: " + cmd);

//       if (cmd == "F"){
//         forward();
//         delay(100);
//         backward();
//       }else if (cmd == "L")
//       {
//         left();
//         delay(100);
//         backward();
//         /* code */
//       }else if (cmd == "R")
//       {
//         right();
//         delay(100);
//         backward();
//         /* code */
//       }else if (cmd == "S")
//       {
//         backward();
//         delay(100);
//         backward();
//         /* code */
//       }else if (cmd == "T")
//       {
//         myservo.write(100);
//         delay(5000);
//         myservo.write(0);
//       }else if (cmd == "P") // p = start motor data enable motor
//       {
//         Mstatus = true;
//       }else if (cmd == "O") // O = stop motor or disable motor
//       {
//         Mstatus = false;
//       }
      

//       // manualMode = true;

//       // if (cmd == "F") forward();
//       // else if (cmd == "L") left();
//       // else if (cmd == "R") right();
//       // else if (cmd == "B") backward();
//       // else if (cmd == "S") {
//       //   manualMode = false;
//       // }
//       // else if (cmd == "T") trunck();
//     }
//   }
// };

// // ---------------- BLE INIT ----------------
// void initBLE()
// {
//   BLEDevice::init("ESP32-Robot");

//   BLEServer *server = BLEDevice::createServer();
//   BLEService *service = server->createService(SERVICE_UUID);

//   pCharacteristic = service->createCharacteristic(
//                       CHARACTERISTIC_UUID,
//                       BLECharacteristic::PROPERTY_WRITE
//                     );

//   pCharacteristic->setCallbacks(new MyCallbacks());

//   service->start();
//   BLEDevice::startAdvertising();

//   Serial.println("BLE Ready");
// }

// // ---------------- TASK ----------------
// void TaskCar(void *pvParameters)
// {
//   while (true)
//   {
//     if(Mstatus){
//       car_motion();
//       trunck();
//       vTaskDelay(50 / portTICK_PERIOD_MS);
//     }
//   }
// }

// // ---------------- SETUP ----------------
// void setup()
// {
//   Serial.begin(115200);

//   pinMode(enb1,OUTPUT);
//   pinMode(enb2,OUTPUT);

//   pinMode(M1,OUTPUT);
//   pinMode(M2,OUTPUT);
//   pinMode(M3,OUTPUT);
//   pinMode(M4,OUTPUT);

//   pinMode(sensorLIP,INPUT);
//   pinMode(sensorRIP,INPUT);

//   myservo.attach(servoPin);

//   if (!vl.begin()) {
//     Serial.println("VL6180X fail");
//     while (1);
//   }

//   myservo.write(0);

//   // TFT
//   tft.begin();
//   tft.setRotation(1);
//   tft.fillScreen(TFT_BLACK);

//   gif.begin(BIG_ENDIAN_PIXELS);

//   // BLE START
//   initBLE();

//   // RTOS TASK
//   xTaskCreatePinnedToCore(
//     TaskCar,
//     "CarTask",
//     2048,
//     NULL,
//     2,
//     NULL,
//     0
//   );
// }

// // ---------------- LOOP (DISPLAY) ----------------
// void loop()
// {
//   for (int i = 0; i < gifCount; i++)
//   {
//     gifPlayer(gifs[i].data, gifs[i].size);
//   }
//   // gifPlayer(gifs[0].data, gifs[0].size);
  
// }


///////////////////////////////////////////


// #include <ESP32Servo.h>
// #include <Wire.h>
// #include "Adafruit_VL6180X.h"

// #include <AnimatedGIF.h>
// #include <SPI.h>
// #include <TFT_eSPI.h>

// #include "sideview.h"
// #include "sleep.h"

// // ---------------- BLE ----------------
// #include <BLEDevice.h>
// #include <BLEServer.h>
// #include <BLEUtils.h>
// #include <BLE2902.h>

// // ---------------- PINS ----------------
// #define M1  26    
// #define M2  27
// #define M3  14
// #define M4  12

// #define enb1  25
// #define enb2  33

// #define sensorLIP 35
// #define sensorRIP 32

// int cnt = 0;
// bool CMDStatus = false;
// bool light = false;

// // ---------------- OBJECTS ----------------
// TFT_eSPI tft = TFT_eSPI();
// AnimatedGIF gif;
// Adafruit_VL6180X vl = Adafruit_VL6180X();
// Servo myservo;

// // ---------------- STATE ----------------
// enum State { IDLE, RUNNING };
// volatile State state = IDLE;

// // ---------------- LIGHT ----------------
// uint8_t threshold = 10;
// bool sleepyPlayed = false;
// bool sidePlayed = false;

// // ---------------- GIF ----------------
// struct GifItem {
//   const uint8_t* data;
//   size_t size;
// };

// GifItem gifs[] = {
//   {sideview, sizeof(sideview)},
//   {sleepy, sizeof(sleepy)}
// };

// uint16_t usTemp[1][256];

// // ---------------- MOTOR ----------------
// void motorsOff(){
//   digitalWrite(enb1, LOW);
//   digitalWrite(enb2, LOW);
// }

// void motorsOn(){
//   digitalWrite(enb1, HIGH);
//   digitalWrite(enb2, HIGH);
// }

// void forward(){
//   digitalWrite(M1,HIGH);
//   digitalWrite(M2,LOW);
//   digitalWrite(M3,HIGH);
//   digitalWrite(M4,LOW);
// }

// void left(){
//   digitalWrite(M1,LOW);
//   digitalWrite(M2,HIGH);
//   digitalWrite(M3,HIGH);
//   digitalWrite(M4,LOW); 
// }

// void right(){
//   digitalWrite(M1,HIGH);
//   digitalWrite(M2,LOW);
//   digitalWrite(M3,LOW);
//   digitalWrite(M4,HIGH); 
// }

// void backward(){
//   digitalWrite(M1,LOW);
//   digitalWrite(M2,LOW);
//   digitalWrite(M3,LOW);
//   digitalWrite(M4,LOW); 
// }

// void trunck()
// {
//   static bool open = false;
//   static unsigned long startTime = 0;

//   uint8_t range = vl.readRange();
//   uint8_t ref = 100 ;

//   if ((range > ref/2) && (range <= ref) && !open) {
//     myservo.write(100);
//     startTime = millis();
//     open = true;
//   }

//   if (open && (millis() - startTime > 5000)) {
//     myservo.write(0);
//     open = false;
//   }
// }


// // ---------------- BLE ----------------
// #define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
// #define CHARACTERISTIC_UUID "abcd1234-5678-1234-5678-abcdef123456"

// class MyCallbacks: public BLECharacteristicCallbacks {
//   void onWrite(BLECharacteristic *pCharacteristic) {
//     String cmd = String(pCharacteristic->getValue().c_str());

//     Serial.println("BLE: " + cmd);

//     cnt++;
//     if (cmd == "P" && CMDStatus == false) {
//       state = RUNNING;
//       motorsOn();
//       // if(cnt <= 1){
//         CMDStatus = true;
//         forward();
//         delay(100);
//         backward();
//       //   cnt = 0;
//       // }
//     }

//     if (cmd == "S") {
//       CMDStatus = false;
//       state = IDLE;
//       motorsOff();
//     }
//     if (cmd == "K") {
//       light = false;
//     }
    
//   }
// };

// void initBLE()
// {
//   BLEDevice::init("ESP32-Robot");

//   BLEServer *server = BLEDevice::createServer();
//   BLEService *service = server->createService(SERVICE_UUID);

//   BLECharacteristic *characteristic =
//     service->createCharacteristic(
//       CHARACTERISTIC_UUID,
//       BLECharacteristic::PROPERTY_WRITE
//     );

//   characteristic->setCallbacks(new MyCallbacks());

//   service->start();
//   BLEDevice::startAdvertising();
// }

// // ---------------- GIF ----------------
// void GIFDraw(GIFDRAW *pDraw)
// {
//   uint8_t *s = pDraw->pPixels;
//   uint16_t *d = usTemp[0];
//   uint16_t *palette = pDraw->pPalette;

//   for (int i = 0; i < pDraw->iWidth; i++)
//     d[i] = palette[*s++];

//   tft.setAddrWindow(pDraw->iX, pDraw->iY + pDraw->y, pDraw->iWidth, 1);
//   tft.pushPixels(d, pDraw->iWidth);
// }

// void gifPlayer(const uint8_t* data, size_t size)
// {
//   if (gif.open((uint8_t*)data, size, GIFDraw))
//   {
//     tft.startWrite();

//     while (gif.playFrame(false, NULL))
//     {
//       vTaskDelay(10 / portTICK_PERIOD_MS); // IMPORTANT
//     }

//     gif.close();
//     tft.endWrite();
//   }
// }


// // ---------------- SENSOR LOGIC ----------------
// void carLogic()
// {
//   if (state != RUNNING) return;

//   int sensorL = digitalRead(sensorLIP);
//   int sensorR = digitalRead(sensorRIP);

//   digitalWrite(enb1,HIGH);
//   digitalWrite(enb2,HIGH);

//   if(sensorL == 0 && sensorR == 0){
//     forward();
//     // light = false;
//   }
//   else if(sensorL == 0 && sensorR == 1){
//     left();
//     // light = false;
//   }
//   else if(sensorL == 1 && sensorR == 0){
//     right();
//     // light = false;
//   }
//   else if(sensorL == 1 && sensorR == 1){
//     backward();
//     light = true;
//   }
//   else{
//     backward();
//   }

//   // int L = digitalRead(sensorLIP);
//   // int R = digitalRead(sensorRIP);

//   // if (L == 0 && R == 0) forward();
//   // else forward();
// }

// // ---------------- LIGHT CHECK ----------------
// void lightLogic()
// {
//   if (state != RUNNING) return;

//   uint8_t val = vl.readLux(VL6180X_ALS_GAIN_5);

//   Serial.println(val);

//   // DARK → sleepy
//   if (val < threshold && !sleepyPlayed)
//   {
//     Serial.println("Sleepy");
//     gifPlayer(gifs[1].data, gifs[1].size);
//     sleepyPlayed = true;
//     sidePlayed = false;
//   }

//   // BRIGHT → sideview
//   if (val >= threshold && !sidePlayed)
//   {
//     Serial.println("Sideview");
//     gifPlayer(gifs[0].data, gifs[0].size);
//     sidePlayed = true;
//     sleepyPlayed = false;
//   }
// }

// // ---------------- TASK ----------------
// void TaskRobot(void *pv)
// {
//   while (true)
//   {
//     if(light == true){
//      tft.setTextSize(3);
//     tft.setTextColor(TFT_BLUE);
//     tft.setCursor(10, 80);
//     tft.println("STOP");
//     motorsOff();
//   }else{
//     motorsOn();
//     lightLogic();
//   }
//   // lightLogic();
//   carLogic();
//   trunck();
//     vTaskDelay(10 / portTICK_PERIOD_MS);  // watchdog safe
//     yield();
//   }
// }


// // ---------------- SETUP ----------------
// void setup()
// {
//   Serial.begin(115200);

//   pinMode(enb1, OUTPUT);
//   pinMode(enb2, OUTPUT);
//   pinMode(M1, OUTPUT);
//   pinMode(M2, OUTPUT);
//   pinMode(M3, OUTPUT);
//   pinMode(M4, OUTPUT);

//   pinMode(sensorLIP, INPUT);
//   pinMode(sensorRIP, INPUT);

//   motorsOff();

//   myservo.attach(16);
//   vl.begin();

//   tft.begin();
//   tft.setRotation(1);
//   tft.fillScreen(TFT_BLACK);
//   gif.begin(BIG_ENDIAN_PIXELS);

     
//     tft.setTextSize(3);
//     tft.setTextColor(TFT_BLUE);
//     tft.setCursor(10, 80);
//     tft.println("2.4 inch TFT");

//     delay(1000);

//   initBLE();

//   xTaskCreatePinnedToCore(
//     TaskRobot,
//     "RobotTask",
//     8192,      // IMPORTANT FIX (stack crash fix)
//     NULL,
//     1,
//     NULL,
//     0
//   );
// }

// // ---------------- LOOP (ONLY DISPLAY) ----------------
// void loop()
// {
//   // idle safe loop (no blocking RTOS conflict)
// }



// #define CUSTOM_SETTINGS
// #define INCLUDE_TERMINAL_MODULE
// #include<Arduino.h>
// #include <DabbleESP32.h>
// String Serialdata = "";
// bool dataflag = 0;
// void setup() {
//   Serial.begin(115200);       // make sure your Serial Monitor is also set at this baud rate.
//   Dabble.begin("MyEsp32");    //set bluetooth name of your device
// }

// void loop() {

//   Dabble.processInput();             //this function is used to refresh data obtained from smartphone.Hence calling this function is mandatory in order to get data properly from your mobile.
//   while (Serial.available() != 0)
//   {
//     Serialdata = String(Serialdata + char(Serial.read()));
//     dataflag = 1;
//   }
//   if (dataflag == 1)
//   {
//     Terminal.print(Serialdata);
//     Serialdata = "";
//     dataflag = 0;
//   }
//   if (Terminal.available() != 0)
//   {
//     while (Terminal.available() != 0)
//     {
//       Serial.write(Terminal.read());
//     }
//     Serial.println();
//   }
// }






