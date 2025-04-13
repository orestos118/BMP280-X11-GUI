#include <Wire.h>
#include <Adafruit_BMP280.h>

Adafruit_BMP280 bmp;

float temperature;
float pressure;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(9600);

  if (!bmp.begin(0x76) && !bmp.begin(0x77)) {
    Serial.println("BMP280 not found!");
    digitalWrite(LED_BUILTIN, HIGH);  // Turn on LED if sensor not found
    while (1);
  }

  bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                  Adafruit_BMP280::SAMPLING_X2,
                  Adafruit_BMP280::SAMPLING_X16,
                  Adafruit_BMP280::FILTER_X16,
                  Adafruit_BMP280::STANDBY_MS_500);
}

void loop() {
  temperature = bmp.readTemperature();
  pressure = bmp.readPressure() / 100.0F;
  float altitude = bmp.readAltitude(1013.25);

  Serial.print("Temp: "); Serial.print(temperature); Serial.println(" °C");
  Serial.print("Pressure: "); Serial.print(pressure); Serial.println(" hPa");
  Serial.print("Altitude: "); Serial.print(altitude); Serial.println(" m");
  Serial.println();

  delay(2000);
}
