// Board needed: Atmel atmega328pb Xplained mini

#include <LiquidCrystal_I2C.h>
#include "DHT.h"

// Pin configuration for DHT11 sensor
#define DHT_PIN 2  // DHT11 connected to D2 (Arduino Nano pin)
#define DHT_TYPE DHT11  // Define sensor type (DHT11)

LiquidCrystal_I2C lcd(0x27, 16, 2);  // LCD with I2C address 0x27, 16 columns, and 2 rows
DHT dht11(DHT_PIN, DHT_TYPE);  // Declare DHT11 sensor object with specified pin and type

void setup() {
  dht11.begin();    // Initialise the DHT11 sensor
  lcd.init();       // Initialise LCD display
  lcd.backlight();  // Turn on the LCD backlight
}

void loop() {
  delay(2000);  // Wait for 2 seconds between readings

  // Read humidity and temperature values from DHT11 sensor
  float humidity = dht11.readHumidity();
  float temperature_C = dht11.readTemperature();

  // Clear LCD to display new data
  lcd.clear();

  // Check if the readings are valid
  if (isnan(humidity) || isnan(temperature_C)) {

    lcd.setCursor(0, 0);  // First row
    lcd.print("Failed");  // Display error if readings fail

  } else {
    // Display temperature
    lcd.setCursor(0, 0);  // First row
    lcd.print("Temperature: ");
    lcd.print(temperature_C);
    lcd.print((char)223);      // Display degree symbol (°)
    lcd.print("C");

    // Display humidity
    lcd.setCursor(0, 1); // Second row
    lcd.print("Humidity: ");
    lcd.print(humidity);
    lcd.print("%");
  }
}
