#include "HttpUpstream.h"
#include <WiFi.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

// todo: device id must be entered in c8y web UI -> display mac address, which is used as device id in console

/**
 * \mainpage
 *
 * \section Introduction
 *
 * This is a library for uploading data from an Arduino micro controller to Cumulocity. This was developed for the [IoT Education Package (IoTEP)](https://education.softwareag.com/internet-of-things).
 *
 * \section Installation
 *
 * This library is on the Arduino Library Manager list.
 * You can install it in the Arduino IDE at Tools -> Manage Libraries... Search for and install _Cumulocity IoT Upstreaming_.
 *
 * \section Limitations
 *
 * In its current state this library does only offer a limited set of capabilities, which are important for IoTEP tutorials.
 *
 * Currently this library does not follow security best practices. Instead of using device specific credentials, it uses user credentials.
 */

// Implementations notes
//
// We use C string formatting for constructing JSON bodies instead of something like
// ArduinoJSON, because
// * We could not figure out how to use ArduinoJSON,
//   more specifically, how to allocate exactly the required amount of memory
// * it is efficient

HttpUpstreamClient::HttpUpstreamClient(Client &networkClient)
{
  _networkClient = &networkClient;
}

// Base64 encoder
/**
 * \brief Stores device credentials for further authentication
 *
 * @param tenantId
 * @param username
 * @param password
 */
int HttpUpstreamClient::storeDeviceCredentialsAndHost(char *host, const char *tenantId, const char *username, const char *password)
{
  _host = host;

  char deviceCredentials[strlen(tenantId) +
                         1 + // "/"
                         strlen(username) +
                         1 + // ":"
                         strlen(password) +
                         1 // string terminator
  ];

  strcpy(deviceCredentials, tenantId);
  strcat(deviceCredentials, "/");
  strcat(deviceCredentials, username);
  strcat(deviceCredentials, ":");
  strcat(deviceCredentials, password);
  Serial.print("Should store deviceCredentials: ");
  Serial.println(deviceCredentials);

#if defined(ARDUINO_ARCH_ESP32)
  String encodedString = base64::encode(deviceCredentials);

  // Memory allocation
  if (_deviceCredentials)
    free(_deviceCredentials);
  _deviceCredentials = (char *)malloc(sizeof(char) * encodedString.length() + 1);
  strcpy(_deviceCredentials, encodedString.c_str());
#else
  int inputStringLength = strlen(deviceCredentials);
  int encodedLength = Base64.encodedLength(inputStringLength);
  char encodedString[encodedLength];
  Base64.encode(encodedString, deviceCredentials, inputStringLength);

  // Memory allocation
  if (_deviceCredentials)
    free(_deviceCredentials);
  _deviceCredentials = (char *)malloc(sizeof(char) * strlen(encodedString));
  strcpy(_deviceCredentials, encodedString);
#endif
  // EEPROM memory layout is
  // * 4 bytes for host length
  // * 4 bytes for credentials length
  // * host string
  // * credentials string

  // todo: start remove
  Serial.println("---------------------");
  Serial.println("Storing into EEPROM...");
  Serial.print("_host: ");
  Serial.println(_host);
  Serial.print("_deviceCredentials: ");
  Serial.println(_deviceCredentials);
  Serial.println("---------------------");
  // todo: end remove

  // todo: check whether uint16_t works as well, that would save 4 bytes EEPROM total, maybe even uint8_t, because credentials and URL should be <= 255 characters?
  uint32_t hostLength = strlen(_host) + 1;
  uint32_t deviceCredentialsLength = strlen(_deviceCredentials) + 1;

  if (hostLength > 512 || deviceCredentialsLength > 512 || hostLength + deviceCredentialsLength + 8 > 512)
  {
    Serial.println("Combination of host and device credentials too long for EEPROM.");
    Serial.println("Host and device credentials will not be persisted.");
    return 1;
  }

  Serial.print("hostLength: ");
  Serial.println(hostLength);
  EEPROM.put(0, hostLength);
  for (int i = 0; i < hostLength; i++)
  {
    EEPROM.write(8 + i, _host[i]);
  }

  Serial.print("deviceCredentialsLength: ");
  Serial.println(deviceCredentialsLength);
  EEPROM.put(4, deviceCredentialsLength);
  for (int i = 0; i < deviceCredentialsLength; i++)
  {
    EEPROM.write(8 + hostLength + i, _deviceCredentials[i]);
  }
  Serial.print("8 + hostLength: ");
  Serial.println(8 + hostLength);

#if defined(ARDUINO_ARCH_ESP32)
  Serial.println("Committing to EEPROM.");
  EEPROM.commit();
#endif
  return 0;
}

/**
 * @brief Loads device credentials and host from EEPROM and puts it into corresponding private vars.
 *
 * Host is loaded into _host
 *
 * Device credentials are loaded into _deviceCredentials
 *
 * @return int 0 = success, 1 = Could not get host and device credentials from EEPROM
 */
int HttpUpstreamClient::loadDeviceCredentialsAndHostFromEEPROM()
{
  // * 4 bytes for credentials length
  // * host string
  // * credentials string

  uint32_t hostLength;
  EEPROM.get(0, hostLength);
  uint32_t deviceCredentialsLength;
  EEPROM.get(4, deviceCredentialsLength);

  Serial.print("hostLength: ");
  Serial.println(hostLength);
  Serial.print("deviceCredentialsLength: ");
  Serial.println(deviceCredentialsLength);

  // If one of hostLength or deviceCredentialsLength is near 2^32 - 1, an overflow would be possible.
  // This should only occur in the case where EEPROM is in cleared state,
  // because EEPROM size is too small for hostLength or deviceCredentialsLength to be big.
  // as host and deviceCredentials must fit into EEPROM.
  if (hostLength > 512 || deviceCredentialsLength > 512 || hostLength + deviceCredentialsLength + 8 > 512)
  {
    return 1;
  }

  // Get host from EEPROM
  if (_host)
    free(_host);
  _host = (char *)malloc(sizeof(char) * hostLength);
  for (int i = 0; i < hostLength; i++)
  {
    _host[i] = EEPROM.read(8 + i);
  }

  // Get device credentials from EEPROM
  if (_deviceCredentials)
    free(_deviceCredentials);
  _deviceCredentials = (char *)malloc(sizeof(char) * deviceCredentialsLength);
  for (int i = 0; i < deviceCredentialsLength; i++)
  {
    _deviceCredentials[i] = EEPROM.read(8 + hostLength + i);
  }
  Serial.print("8 + hostLength: ");
  Serial.println(8 + hostLength);

  if (hostLength > 0 && deviceCredentialsLength > 0)
  {
    int contentLength = 53 + hostLength + 1;
    char message[contentLength];
    snprintf_P(message, contentLength, PSTR("Got host and device credentials from EEPROM. Host is %s"), _host);
    Serial.println(message);

    // todo: start remove
    Serial.println("---------------------");
    Serial.println("Loaded from EEPROM...");
    Serial.print("_host: ");
    Serial.println(_host);
    Serial.print("_deviceCredentials: ");
    Serial.println(_deviceCredentials);
    Serial.println("---------------------");
    // todo: end remove

    // Success
    return 0;
  }
  // Could not get host and device credentials from EEPROM
  return 2;
}

/**
 * @brief Removes device from tenant.
 *
 * Removes device from tenant and clears EEPROM, which stores tenant host and device credentials.
 *
 * Will not clear EEPROM in case the device cannot be removed from the tenant.
 */
void HttpUpstreamClient::removeDevice()
{
  removeDevice(false);
}

/**
 * @brief Removes device from tenant.
 *
 * Removes device from tenant and clears EEPROM, which stores tenant host and device credentials.
 *
 * You can specify whether to force clear EEPROM when the device cannot be removed from the tenant. (E.g. in case you already removed it manually.)
 *
 * @param forceClearEEPROM whether to clear EEPROM even when device cannot be removed from the tenant.
 */
void HttpUpstreamClient::removeDevice(bool forceClearEEPROM)
{
  int status = loadDeviceCredentialsAndHostFromEEPROM();
  if (status == 1)
  {
    Serial.println("Was unable to load host and device credentials from EEPROM.");
    if (forceClearEEPROM)
      Serial.println("Force clearing EEPROM...");
  }
  else
  {
    // todo: do HTTP stuff to remove device
  }
  if (!status || forceClearEEPROM)
  {
    for (int i = 0; i < EEPROM.length(); i++)
    {
#if defined(ARDUINO_ARCH_ESP32)
      EEPROM.write(i, 255);
#else
      EEPROM.update(i, 255);
#endif
    }
#if defined(ARDUINO_ARCH_ESP32)
    EEPROM.commit();
#endif
  }
}

/**
 * @brief
 *
 * @param host Cumulocity tenant domain name, e.g. iotep.cumulocity.com
 * @return int 0 = success
 */
int HttpUpstreamClient::requestDeviceCredentialsFromTenant(char *host)
{
#if defined(ARDUINO_ARCH_ESP32)
  String id = String(WiFi.macAddress());
  id.replace(":", "_");
#else
  byte mac[6];
  WiFi.macAddress(mac);
  char id_c_str[18];
  snprintf_P(id_c_str, 18, PSTR("%02X_%02X_%02X_%02X_%02X_%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  String id = String(id_c_str);
#endif
  int contentLength =
      id.length() +
      9 + // length of template string without placeholders
      1;  // probably the \n by println(body2send) instead of print(body2send) ?
  char body2send[contentLength];
  snprintf_P(body2send, contentLength, PSTR("{\"id\":\"%s\"}"), id.c_str());

  // todo: think about timeout
  String msg = "";
  while (true)
  {
    if (_networkClient->connected())
      _networkClient->stop();
    if (_networkClient->connect(host, 443))
    {
      Serial.println("Requesting device credentials...");
      Serial.print("Please register a new device with device ID ");
      Serial.print(id);
      Serial.println(" in your tenant.");

      _networkClient->println("POST /devicecontrol/deviceCredentials HTTP/1.1");
      _networkClient->print("Host: ");
      _networkClient->println(host);
      _networkClient->println("Authorization: Basic bWFuYWdlbWVudC9kZXZpY2Vib290c3RyYXA6RmhkdDFiYjFm");
      _networkClient->println("Content-Type: application/json");
      _networkClient->print("Content-Length: ");
      _networkClient->println(contentLength);
      _networkClient->println("Accept: application/json");
      _networkClient->println();
      _networkClient->println(body2send);
      _networkClient->flush();
    }

    msg = "";
    while (_networkClient->connected())
    {
      while (_networkClient->available())
      {
        char c = _networkClient->read();
        msg += c;
      }
      if (msg.length() > 0)
      {
        // todo: Check for empty messages, possibly more nuanced error handling;
        if (msg.startsWith("HTTP/1.1 404 Not Found"))
        {
          _networkClient->stop();
        }
        else if (msg.indexOf(String("{")) == -1)
        {
        }
        else
        {
          while (!msg.startsWith(String("\r\n\r\n")))
          {
            msg.remove(0, 1);
          }
          msg.remove(0, 4);
          DynamicJsonDocument doc(msg.length() + 101);
          deserializeJson(doc, msg);
          storeDeviceCredentialsAndHost(host, doc["tenantId"], doc["username"], doc["password"]);
          _networkClient->stop();
          return 0;
        }
      }
      delay(100);
    }
    delay(3000);
  }
}

// Register device on the cloud and obtain the device id
/**
 * @brief Registers the device with Cumulocity.
 *
 * Will busy wait for you to accept the device in device management.
 *
 * This method will store the host and device credentials in EEPROM. If the host argument is identical to the host in EEPROM, the device will be assumed to be already registered with your tenant.
 * If the tenant state does not match this assumption, the method will return an error code. In this case, load a sketch, which calls _removeDevice_.
 *
 * @param deviceName
 * @return int 0 = success
 */
int HttpUpstreamClient::registerDevice(char *host, char *deviceName)
{
  char *supportedOperations[0];
  return registerDevice(host, deviceName, supportedOperations);
}

/**
 * @brief Registers the device with Cumulocity.
 *
 * Sames as above, but with supportedOperations
 *
 * @param host Cumulocity tenant domain name, e.g. iotep.cumulocity.com
 * @param deviceName
 * @param supportedOperations
 *
 * @return int 0 = success
 */
int HttpUpstreamClient::registerDevice(char *host, char *deviceName, char *supportedOperations[])
{
#if defined(ARDUINO_ARCH_ESP32)
  Serial.println("EEPROM.begin(512)");
  Serial.println(EEPROM.begin(512));
#endif
  Serial.println("Preparing to register device.");

  int status = loadDeviceCredentialsAndHostFromEEPROM();
  // POSIX exit status convention. 0 = success, 1 to 255 = something else
  if (status == 1)
  {
    Serial.println("Was unable to load host and device credentials from EEPROM. Requesting new device credentials from tenant.");
    status = requestDeviceCredentialsFromTenant(host);
  }
  if (strcmp(_host, host))
  {
    Serial.println("Host changed. Requesting new device credentials from tenant.");
    status = requestDeviceCredentialsFromTenant(host);
  }
  if (status)
  {
    return status;
  }

  // JSON Body
  DynamicJsonDocument body(
      JSON_OBJECT_SIZE(2) +
      strlen(deviceName) + 1 +
      2);
  String body2send = "";
  body["name"] = deviceName;
  body["c8y_IsDevice"] = "{}";
  serializeJson(body, body2send);

  // HTTP header
  if (_networkClient->connected())
    _networkClient->stop();
  if (_networkClient->connect(_host, 443))
  {
    Serial.println("Registering device...");
    _networkClient->println("POST /inventory/managedObjects/ HTTP/1.1");
    _networkClient->print("Host: ");
    _networkClient->println(_host);
    _networkClient->print("Authorization: Basic ");
    _networkClient->println(_deviceCredentials);
    _networkClient->println("Content-Type: application/json");
    _networkClient->print("Content-Length: ");
    _networkClient->println(body2send.length());
    _networkClient->println("Accept: application/json");
    _networkClient->println();
    _networkClient->println(body2send);
    _networkClient->flush();
  }

  // Device ID
  while (_deviceID.length() == 0)
  {
    String msg = "";
    while (_networkClient->available())
    {
      char c = _networkClient->read();
      msg += c;
    }
    int start = msg.indexOf("\"id\"");
    int until = msg.indexOf("\":", start);
    int until_n = msg.indexOf("\",", until);
    if (until != -1 && start != -1)
    {
      _deviceID = msg.substring(until + 3, until_n);
      Serial.print("Device ID for ");
      Serial.print(deviceName);
      Serial.print(" is ");
      Serial.println(_deviceID);
      return 0;
    }
  }
}

// Measurement Type: c8y_Typemeasuremnt
// Measuremnt Name: c8y_measurmentname
/**
 * @brief Sends a measurement.
 *
 * @param value
 * @param unit The unit of the measurement, such as "Wh" or "C".
 * @param timestamp Time of the measurement.
 * @param c8y_measurementType The most specific type of this entire measurement.
 * @param c8y_measurementObjectName
 * @param Name
 * @param host
 */
// todo: unit should be optional
// todo: What about floating point values? c8y doc mentions both 64 bit floats and 64 bit signed integers.
// todo: Name arguments consistently with c8y
void HttpUpstreamClient::sendMeasurement(int value, char *unit, String timestamp, char *type, char *c8y_measurementObjectName, char *Name)
{
  Serial.print("Preparing to send measurement with device ID: ");
  Serial.println(_deviceID);

  int contentLength =
      strlen(Name) +
      strlen(c8y_measurementObjectName) +
      strlen(unit) +
      String(value).length() +
      _deviceID.length() +
      timestamp.length() +
      strlen(type) +
      69 + // length of template string without placeholders
      1;   // string terminator
  char body2send[contentLength];
  snprintf_P(body2send, contentLength, PSTR("{\"%s\":{\"%s\":{\"unit\":\"%s\",\"value\":%i}},\"source\":{\"id\":\"%s\"},\"time\":\"%s\",\"type\":\"%s\"}"), Name, c8y_measurementObjectName, unit, value, _deviceID.c_str(), timestamp.c_str(), type);

  if (_deviceID.length() != 0)
  {
    if (_networkClient->connected())
      _networkClient->stop();
    if (_networkClient->connect(_host, 443))
    {
      Serial.println("Sending measurement...");

      _networkClient->println("POST /measurement/measurements HTTP/1.1");
      _networkClient->print("Host: ");
      _networkClient->println(_host);
      _networkClient->print("Authorization: Basic ");
      _networkClient->println(_deviceCredentials);
      _networkClient->println("Content-Type: application/json");
      _networkClient->print("Content-Length: ");
      _networkClient->println(contentLength);
      _networkClient->println("Accept: application/json");
      _networkClient->println();
      _networkClient->println(body2send);
      _networkClient->flush();
    }
    else
    {
      Serial.print("Could not connect to ");
      Serial.println(_host);
      Serial.println(WiFi.status());
    }
  }
  else
  {
    Serial.println("Device id undefined. Did you register the device?");
  }
}

void HttpUpstreamClient::sendAlarm(char *alarm_Type, char *alarm_Text, char *severity, String timestamp)
{
  Serial.print("Preparing to send alarm with device ID: ");
  Serial.println(_deviceID);

  int contentLength =
      strlen(severity) +
      _deviceID.length() +
      strlen(alarm_Text) +
      timestamp.length() +
      strlen(alarm_Type) +
      64 + // length of template string without placeholders
      1;   // string terminator
  char body2send[contentLength];
  snprintf_P(body2send, contentLength, PSTR("{\"severity\":\"%s\",\"source\":{\"id\":\"%s\"},\"text\":\"%s\",\"time\":\"%s\",\"type\":\"%s\"}"), severity, _deviceID.c_str(), alarm_Text, timestamp.c_str(), alarm_Type);

  if (_deviceID.length() != 0)
  {
    if (_networkClient->connected())
      _networkClient->stop();
    if (_networkClient->connect(_host, 443))
    {
      Serial.println("Sending alarm...");

      _networkClient->println("POST /alarm/alarms HTTP/1.1");
      _networkClient->print("Host: ");
      _networkClient->println(_host);
      _networkClient->print("Authorization: Basic ");
      _networkClient->println(_deviceCredentials);
      _networkClient->println("Content-Type: application/json");
      _networkClient->print("Content-Length: ");
      _networkClient->println(contentLength);
      _networkClient->println("Accept: application/json");
      _networkClient->println();
      _networkClient->println(body2send);
      _networkClient->flush();
    }
  }
}

void HttpUpstreamClient::sendEvent(char *event_Type, char *event_Text, String timestamp)
{
  Serial.print("Preparing to send event with device ID: ");
  Serial.println(_deviceID);

  int contentLength =
      _deviceID.length() +
      strlen(event_Text) +
      timestamp.length() +
      strlen(event_Type) +
      50 + // length of template string without placeholders
      1;   // string terminator
  char body2send[contentLength];
  snprintf_P(body2send, contentLength, PSTR("{\"source\":{\"id\":\"%s\"},\"text\":\"%s\",\"time\":\"%s\",\"type\":\"%s\"}"), _deviceID.c_str(), event_Text, timestamp.c_str(), event_Type);

  if (_deviceID.length() != 0)
  {
    if (_networkClient->connected())
      _networkClient->stop();
    if (_networkClient->connect(_host, 443))
    {
      Serial.println("Sending event...");

      _networkClient->println("POST /event/events HTTP/1.1");
      _networkClient->print("Host: ");
      _networkClient->println(_host);
      _networkClient->print("Authorization: Basic ");
      _networkClient->println(_deviceCredentials);
      _networkClient->println("Content-Type: application/json");
      _networkClient->print("Content-Length: ");
      _networkClient->println(contentLength);
      _networkClient->println("Accept: application/json");
      _networkClient->println();
      _networkClient->println(body2send);
    }
  }
}
