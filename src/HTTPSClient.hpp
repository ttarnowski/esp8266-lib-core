#pragma once

#include <CertStoreBearSSL.h>
#include <ESP8266HTTPClient.h>
#include <EventDispatcher.hpp>
#include <WiFiManager.hpp>

class BodyStream : public Stream {
public:
  BodyStream(WiFiClient *wifiClient, HTTPClient *httpClient) {
    this->wifiClient = wifiClient;
    this->httpClient = httpClient;
    this->bytesLeft = this->httpClient->getSize();
  }
  ~BodyStream() {}

  int available() {
    if (!this->httpClient->connected()) {
      return 0;
    }

    return this->bytesLeft;
  }

  size_t readBytes(uint8_t *buffer, size_t length) {
    if (this->bytesLeft == 0) {
      return 0;
    }

    int bytesRead = this->wifiClient->readBytes(
        buffer, std::min((size_t)this->bytesLeft, length));

    if (this->bytesLeft > 0) {
      this->bytesLeft -= bytesRead;
    }

    return bytesRead;
  }

  size_t write(uint8_t buffer) { return this->wifiClient->write(buffer); }
  int read() { return this->wifiClient->read(); }
  int peek() { return this->wifiClient->peek(); }

  String readString() { return this->wifiClient->readString(); }

private:
  WiFiClient *wifiClient;
  HTTPClient *httpClient;
  int bytesLeft;
};

enum HTTPMethod {
  HTTP_GET,
  HTTP_HEAD,
  HTTP_POST,
  HTTP_PUT,
  HTTP_PATCH,
  HTTP_DELETE,
  HTTP_OPTIONS
};

class RequestBuilder;

struct Request {
  static RequestBuilder build(HTTPMethod method);
  const char *baseUrl;
  const char *path;
  HTTPMethod method;
  const char *body;
  std::vector<std::pair<const char *, const char *>> headers;
};

class RequestBuilder {
public:
  RequestBuilder(HTTPMethod m) { request.method = m; }

  operator Request &&() { return std::move(request); }

  RequestBuilder &baseUrl(const char *u) {
    request.baseUrl = u;
    return *this;
  }

  RequestBuilder &path(const char *p) {
    request.path = p;
    return *this;
  }

  RequestBuilder &body(const char *b) {
    request.body = b;
    return *this;
  }

  RequestBuilder &
  headers(std::vector<std::pair<const char *, const char *>> h) {
    request.headers = h;
    return *this;
  }

private:
  Request request;
};

RequestBuilder Request::build(HTTPMethod method) {
  return RequestBuilder(method);
}

struct Response {
  const char *error;
  int statusCode;
  BodyStream *body;
};

class HTTPSClient {
public:
  HTTPSClient(CertStore *certStore, WiFiManager *wifiManager, Timer *timer) {
    this->wifiManager = wifiManager;
    this->timer = timer;
    this->certStore = certStore;
  }

  void sendRequest(Request request, std::function<void(Response)> onResponse) {
    this->wifiManager->connect([=](wl_status_t status) {
      if (status != WL_CONNECTED) {
        onResponse(Response{"could not connect to WiFi", -1, nullptr});
        return;
      }

      this->setClock([request, onResponse, this](bool success) {
        if (!success) {
          onResponse(Response{"could not synchronize the time", -1, nullptr});
          return;
        }

        BearSSL::WiFiClientSecure client;
        client.setCertStore(this->certStore);

        HTTPClient http;

        Serial.print("[HTTP] begin...\n");

        char url[strlen(request.baseUrl) + strlen(request.path) + 1];

        strcpy(url, request.baseUrl);
        strcat(url, request.path);

        if (http.begin(client, url)) {
          char method[10];
          this->readMethod(request.method, method);

          Serial.printf("[HTTP] %s %s\n", method, url);
          // start connection and send HTTP header, set the HTTP method and
          // request body
          for (auto &h : request.headers) {
            http.addHeader(h.first, h.second);
          }

          int httpCode = http.sendRequest(method, String(request.body));

          // httpCode will be negative on error
          if (httpCode > 0) {
            // HTTP header has been send and Server response header has been
            // handled
            Serial.printf("[HTTP] %s... code: %d\n", method, httpCode);

            BodyStream body(&client, &http);

            onResponse(Response{nullptr, httpCode, &body});
          } else {
            // print out the error message
            Serial.printf("[HTTP] %s... failed, error: %s\n", method,
                          http.errorToString(httpCode).c_str());
            onResponse(Response{http.errorToString(httpCode).c_str(), httpCode,
                                nullptr});
          }

          // finish the exchange
          http.end();
        } else {
          Serial.printf("[HTTP] Unable to connect\n");
          onResponse(Response{"unable to connect", -1, nullptr});
        }
      });
    });
  }

private:
  void setClock(std::function<void(bool)> onClockSet,
                unsigned long timeoutMs = 60000) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    Serial.println("Waiting for NTP time sync: ");

    this->timer->setOnLoopUntil(
        [onClockSet]() {
          time_t now = time(nullptr);
          if (now >= 8 * 3600 * 2) {
            Serial.println("");
            struct tm timeinfo;
            gmtime_r(&now, &timeinfo);
            Serial.print("Current time: ");
            Serial.print(asctime(&timeinfo));
            onClockSet(true);
            return true;
          }

          return false;
        },
        [onClockSet]() { onClockSet(false); }, timeoutMs);
  }

  void readMethod(HTTPMethod method, char *methodValue) {
    switch (method) {
    case HTTP_OPTIONS:
      strcpy(methodValue, "OPTIONS");
      break;

    case HTTP_DELETE:
      strcpy(methodValue, "DELETE");
      break;

    case HTTP_PATCH:
      strcpy(methodValue, "PATCH");
      break;

    case HTTP_PUT:
      strcpy(methodValue, "PUT");
      break;

    case HTTP_POST:
      strcpy(methodValue, "POST");
      break;

    case HTTP_HEAD:
      strcpy(methodValue, "HEAD");
      break;

    default:
      strcpy(methodValue, "GET");
      break;
    }
  }

  WiFiManager *wifiManager;
  Timer *timer;
  CertStore *certStore;
};
