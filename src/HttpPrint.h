// Print adapter that streams into a chunked WebServer response in small blocks, so a long
// page or command output never has to be built in one String. The response must already be
// started with setContentLength(CONTENT_LENGTH_UNKNOWN) + send(); end it with sendContent("").
#pragma once

#include <Arduino.h>
#include <WebServer.h>

class HttpPrint : public Print {
public:
  explicit HttpPrint(WebServer &server) : server_(server) {}
  ~HttpPrint() override { flush(); }

  size_t write(const uint8_t c) override {
    buf_[len_++] = static_cast<char>(c);
    if (len_ == sizeof(buf_) - 1) flush();
    return 1;
  }
  void flush() override {
    if (len_ == 0) return;
    buf_[len_] = '\0';
    server_.sendContent(buf_, len_);
    len_ = 0;
  }

private:
  WebServer &server_;
  char buf_[256]{};
  size_t len_ = 0;
};
